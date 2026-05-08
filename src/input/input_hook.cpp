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
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

XInputGetStateFn g_original_xinput_get_state = nullptr;
GetRawInputDataFn g_original_get_raw_input_data = nullptr;
LPVOID g_xinput_target = nullptr;
LPVOID g_raw_input_target = nullptr;

DWORD WINAPI HookedXInputGetState(DWORD user_index, XINPUT_STATE* state)
{
    const DWORD result = g_original_xinput_get_state(user_index, state);
    if (result == ERROR_SUCCESS) radial_input::HandleControllerState(user_index, state);

    return result;
}

UINT WINAPI HookedGetRawInputData(HRAWINPUT raw_input, UINT command, LPVOID data, PUINT size, UINT header_size)
{
    const UINT result = g_original_get_raw_input_data(raw_input, command, data, size, header_size);
    if (result == static_cast<UINT>(-1) || command != RID_INPUT || data == nullptr || !radial_menu::IsOpen()) {
        return result;
    }

    auto* raw = reinterpret_cast<RAWINPUT*>(data);
    if (raw->header.dwType != RIM_TYPEMOUSE) return result;

    radial_input::AddMouseDelta(static_cast<float>(raw->data.mouse.lLastX), static_cast<float>(raw->data.mouse.lLastY));
    raw->data.mouse.lLastX = 0;
    raw->data.mouse.lLastY = 0;
    return result;
}

bool InstallRawInputHook()
{
    if (g_raw_input_target != nullptr) return true;

    HMODULE module = LoadLibraryW(L"user32.dll");
    if (module == nullptr) return false;

    auto* const target = reinterpret_cast<LPVOID>(GetProcAddress(module, "GetRawInputData"));
    if (target == nullptr) return false;

    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<LPVOID>(&HookedGetRawInputData),
        reinterpret_cast<LPVOID*>(&g_original_get_raw_input_data));
    if (status != MH_OK) {
        Log("Failed to create GetRawInputData hook: %s", MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        Log("Failed to enable GetRawInputData hook: %s", MH_StatusToString(status));
        return false;
    }

    g_raw_input_target = target;
    Log("Installed GetRawInputData hook.");
    return true;
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
        if (!InstallRawInputHook()) Log("Unable to install GetRawInputData hook.");
        return true;
    }

    Log("Unable to locate an XInput DLL to hook.");
    return false;
}

void Shutdown()
{
    if (g_raw_input_target != nullptr) {
        MH_DisableHook(g_raw_input_target);
        MH_RemoveHook(g_raw_input_target);
        g_raw_input_target = nullptr;
        g_original_get_raw_input_data = nullptr;
    }

    if (g_xinput_target == nullptr) return;

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
