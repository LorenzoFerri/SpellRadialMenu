#include "input_hook.h"

#include "common.h"
#include "radial_menu.h"
#include "spell_manager.h"

#include <MinHook.h>
#include <windows.h>
#include <xinput.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace radial_spell_menu::input_hook {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

constexpr ULONGLONG kHoldThresholdMs = 180;
constexpr ULONGLONG kQuickSelectHoldMs = 260;
constexpr ULONGLONG kQuickSelectReleaseGapMs = 100;
constexpr ULONGLONG kTapPressMs = 60;
constexpr ULONGLONG kTapReleaseMs = 90;
constexpr WORD kInterceptedButton = XINPUT_GAMEPAD_DPAD_UP;
constexpr DWORD kControllerCount = 4;

enum class SyntheticInputPhase {
    idle,
    quick_select_hold,
    quick_select_release_gap,
    tap_press,
    tap_release,
};

struct ControllerContext {
    bool dpad_up_down = false;
    bool radial_opened = false;
    ULONGLONG pressed_at = 0;
    SyntheticInputPhase synthetic_phase = SyntheticInputPhase::idle;
    ULONGLONG synthetic_phase_until = 0;
    std::uint32_t remaining_taps = 0;
};

std::array<ControllerContext, kControllerCount> g_controllers = {};
XInputGetStateFn g_original_xinput_get_state = nullptr;
LPVOID g_xinput_target = nullptr;

float NormalizeThumbAxis(SHORT value, SHORT deadzone)
{
    if (std::abs(static_cast<int>(value)) <= deadzone) {
        return 0.0f;
    }

    const float normalized = static_cast<float>(value) / 32767.0f;
    return (normalized < -1.0f) ? -1.0f : ((normalized > 1.0f) ? 1.0f : normalized);
}

void QueueTapSequence(ControllerContext& controller, ULONGLONG now, std::uint32_t tap_count)
{
    if (tap_count == 0) {
        controller.synthetic_phase = SyntheticInputPhase::idle;
        controller.synthetic_phase_until = 0;
        controller.remaining_taps = 0;
        return;
    }

    controller.synthetic_phase = SyntheticInputPhase::tap_press;
    controller.synthetic_phase_until = now + kTapPressMs;
    controller.remaining_taps = tap_count;
}

void QueueQuickSelectSequence(ControllerContext& controller, ULONGLONG now, std::uint32_t taps_after_first_spell)
{
    controller.synthetic_phase = SyntheticInputPhase::quick_select_hold;
    controller.synthetic_phase_until = now + kQuickSelectHoldMs;
    controller.remaining_taps = taps_after_first_spell;
}

void ApplyVirtualTap(ControllerContext& controller, XINPUT_STATE* state, ULONGLONG now)
{
    state->Gamepad.wButtons &= ~kInterceptedButton;

    switch (controller.synthetic_phase) {
    case SyntheticInputPhase::idle:
        return;

    case SyntheticInputPhase::quick_select_hold:
        if (now < controller.synthetic_phase_until) {
            state->Gamepad.wButtons |= kInterceptedButton;
            return;
        }

        controller.synthetic_phase = SyntheticInputPhase::quick_select_release_gap;
        controller.synthetic_phase_until = now + kQuickSelectReleaseGapMs;
        return;

    case SyntheticInputPhase::quick_select_release_gap:
        if (now < controller.synthetic_phase_until) {
            return;
        }

        if (controller.remaining_taps == 0) {
            controller.synthetic_phase = SyntheticInputPhase::idle;
            controller.synthetic_phase_until = 0;
            return;
        }

        controller.synthetic_phase = SyntheticInputPhase::tap_press;
        controller.synthetic_phase_until = now + kTapPressMs;
        state->Gamepad.wButtons |= kInterceptedButton;
        return;

    case SyntheticInputPhase::tap_press:
        if (controller.remaining_taps == 0) {
            controller.synthetic_phase = SyntheticInputPhase::idle;
            controller.synthetic_phase_until = 0;
            return;
        }

        state->Gamepad.wButtons |= kInterceptedButton;
        if (now >= controller.synthetic_phase_until) {
            --controller.remaining_taps;
            controller.synthetic_phase = SyntheticInputPhase::tap_release;
            controller.synthetic_phase_until = now + kTapReleaseMs;
        }
        return;

    case SyntheticInputPhase::tap_release:
        if (now < controller.synthetic_phase_until) {
            return;
        }

        if (controller.remaining_taps == 0) {
            controller.synthetic_phase = SyntheticInputPhase::idle;
            controller.synthetic_phase_until = 0;
            return;
        }

        controller.synthetic_phase = SyntheticInputPhase::tap_press;
        controller.synthetic_phase_until = now + kTapPressMs;
        state->Gamepad.wButtons |= kInterceptedButton;
        return;
    }
}

DWORD WINAPI HookedXInputGetState(DWORD user_index, XINPUT_STATE* state)
{
    const DWORD result = g_original_xinput_get_state(user_index, state);
    if (result != ERROR_SUCCESS || state == nullptr || user_index >= g_controllers.size()) {
        return result;
    }

    auto& controller = g_controllers[user_index];
    const ULONGLONG now = GetTickCount64();
    const bool physical_dpad_up_pressed = (state->Gamepad.wButtons & kInterceptedButton) != 0;

    if (physical_dpad_up_pressed && !controller.dpad_up_down) {
        controller.dpad_up_down = true;
        controller.radial_opened = false;
        controller.pressed_at = now;
    }

    if (controller.dpad_up_down && physical_dpad_up_pressed && !controller.radial_opened &&
        (now - controller.pressed_at) >= kHoldThresholdMs) {
        controller.radial_opened = true;
        const auto spell_slots = GetMemorizedSpells();
        int initial_selection = GetCurrentSpellSlot();
        if (initial_selection < 0 && !spell_slots.empty()) {
            initial_selection = 0;
        }

        radial_spell_menu::radial_menu::Open(initial_selection);
        Log("Opening radial spell menu for controller %lu.", static_cast<unsigned long>(user_index));
    }

    if (controller.radial_opened) {
        const auto spell_slots = GetMemorizedSpells();
        const float right_x = NormalizeThumbAxis(state->Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        const float right_y = NormalizeThumbAxis(state->Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);

        radial_spell_menu::radial_menu::UpdateSelectionFromStick(right_x, right_y, spell_slots.size());

        state->Gamepad.sThumbRX = 0;
        state->Gamepad.sThumbRY = 0;
    }

    if (controller.dpad_up_down && !physical_dpad_up_pressed) {
        if (controller.radial_opened) {
            const int selected_slot = radial_spell_menu::radial_menu::GetSelectedSlot();
            const auto spell_slots = GetMemorizedSpells();

            if (selected_slot >= 0 && selected_slot < static_cast<int>(spell_slots.size())) {
                const std::size_t selected_index = static_cast<std::size_t>(selected_slot);
                if (!SwitchToSpellSlot(spell_slots[selected_index].slot_index)) {
                    QueueQuickSelectSequence(controller, now, static_cast<std::uint32_t>(selected_index));
                    Log("Queued quick-select plus %u follow-up taps for spell selection.",
                        static_cast<unsigned int>(selected_index));
                }
            }

            radial_spell_menu::radial_menu::Close();
            Log("Closed radial spell menu for controller %lu.", static_cast<unsigned long>(user_index));
        } else {
            QueueTapSequence(controller, now, 1);
        }

        controller.dpad_up_down = false;
        controller.radial_opened = false;
        state->Gamepad.wButtons &= ~kInterceptedButton;
    }

    if (controller.dpad_up_down || controller.radial_opened) {
        state->Gamepad.wButtons &= ~kInterceptedButton;
    }

    ApplyVirtualTap(controller, state, now);

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
    radial_spell_menu::radial_menu::Close();
    g_controllers = {};
    g_xinput_target = nullptr;
    g_original_xinput_get_state = nullptr;
}

bool IsMenuOpen()
{
    return radial_spell_menu::radial_menu::IsOpen();
}

}  // namespace radial_spell_menu::input_hook
