#include "game/input/radial_switch.h"

#include "core/common.h"
#include "game/equipment/equip_access.h"
#include "game/input/in_game_pad.h"
#include "game/input/radial_camera.h"
#include "game/state/gameplay_state.h"
#include "input/radial_input.h"
#include "render/ui/radial_menu.h"

#include <MinHook.h>
#include <cstdint>

namespace radial_menu_mod::radial_switch {
namespace {

constexpr std::uintptr_t kEquipmentHudUpdateRva = 0x7757A0;
constexpr std::uintptr_t kSwitchItemRequestCheckRva = 0x758350;
constexpr std::uintptr_t kSwitchSpellRequestCheckRva = 0x7583C0;
constexpr std::uintptr_t kSwitchSpellHoldCheckRva = 0x758510;
constexpr std::uintptr_t kSwitchItemRepeatCheckRva = 0x758670;
constexpr std::uintptr_t kSwitchSpellRepeatCheckRva = 0x758920;
constexpr std::uintptr_t kCanSwitchSpellRva = 0x250850;
constexpr std::uintptr_t kSwitchItemNextRva = 0x24FED0;
constexpr std::uintptr_t kSwitchSpellNextRva = 0x250E60;
constexpr std::uintptr_t kEquipmentChangeSoundEventRva = 0x814FC0;
constexpr std::uintptr_t kEquipmentChangeSoundEventVtableRva = 0x2A9DBD0;

constexpr std::int32_t kSwitchSpell2Input = 24;
constexpr std::int32_t kSwitchItem2Input = 25;
constexpr std::int32_t kSwitchSpellAction = 13;
constexpr std::int32_t kSwitchItemAction = 14;
constexpr DWORD kRadialHoldThresholdMs = 220;

struct EquipmentChangeSoundEvent {
    std::int32_t event_id = 0x2710;
    std::int32_t target = -1;
    std::int32_t category = 3;
    std::int32_t padding = 0;
    void* vtable = nullptr;
};

struct CaptureState {
    bool input_down = false;
    bool capture_active = false;
    bool radial_started = false;
    bool tap_feedback_pending = false;
    ULONGLONG pressed_ms = 0;
};

using EquipmentHudUpdateFn = void (*)(void* hud_context, void* arg2, void* arg3);
using InputRequestCheckFn = bool (*)(void* input_state);
using InputHoldCheckFn = bool (*)(void* input_state, std::int32_t action);
using CanSwitchSpellFn = bool (*)(void* equip_magic_data);
using SwitchSpellNextFn = void (*)(void* equip_magic_data);
using SwitchItemNextFn = void (*)(void* equip_item_data);
using EquipmentChangeSoundEventFn = void (*)(EquipmentChangeSoundEvent* event);

CaptureState g_spell_capture = {};
CaptureState g_item_capture = {};

bool g_equipment_hud_update_hook_installed = false;
bool g_equipment_hud_update_hook_failed = false;
bool g_switch_spell_request_hook_installed = false;
bool g_switch_spell_request_hook_failed = false;
bool g_switch_item_request_hook_installed = false;
bool g_switch_item_request_hook_failed = false;
bool g_switch_hold_hook_installed = false;
bool g_switch_hold_hook_failed = false;
bool g_switch_spell_repeat_hook_installed = false;
bool g_switch_spell_repeat_hook_failed = false;
bool g_switch_item_repeat_hook_installed = false;
bool g_switch_item_repeat_hook_failed = false;
bool g_can_switch_spell_hook_installed = false;
bool g_can_switch_spell_hook_failed = false;
bool g_switch_spell_next_hook_installed = false;
bool g_switch_spell_next_hook_failed = false;
bool g_switch_item_next_hook_installed = false;
bool g_switch_item_next_hook_failed = false;

bool g_logged_switch_spell_request_suppression = false;
bool g_logged_switch_item_request_suppression = false;
bool g_logged_switch_spell_hold_suppression = false;
bool g_logged_switch_item_hold_suppression = false;
bool g_logged_switch_spell_repeat_suppression = false;
bool g_logged_switch_item_repeat_suppression = false;
bool g_logged_can_switch_spell_suppression = false;
bool g_logged_switch_spell_next_suppression = false;
bool g_logged_switch_item_next_suppression = false;
bool g_logged_switch_spell_tap_passthrough = false;
bool g_logged_switch_item_tap_passthrough = false;

EquipmentHudUpdateFn g_original_equipment_hud_update = nullptr;
InputRequestCheckFn g_original_switch_spell_request_check = nullptr;
InputRequestCheckFn g_original_switch_item_request_check = nullptr;
InputHoldCheckFn g_original_switch_hold_check = nullptr;
InputRequestCheckFn g_original_switch_spell_repeat_check = nullptr;
InputRequestCheckFn g_original_switch_item_repeat_check = nullptr;
CanSwitchSpellFn g_original_can_switch_spell = nullptr;
SwitchSpellNextFn g_original_switch_spell_next = nullptr;
SwitchItemNextFn g_original_switch_item_next = nullptr;

template <typename T>
bool WriteGameMemory(std::uintptr_t address, const T& value)
{
    if (!IsReadableMemory(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    *reinterpret_cast<T*>(address) = value;
    return true;
}

void BeginCapture(CaptureState& capture)
{
    if (capture.input_down) return;
    capture.input_down = true;
    capture.capture_active = true;
    capture.radial_started = false;
    capture.pressed_ms = GetTickCount64();
}

void ResetCapture(CaptureState& capture)
{
    capture.input_down = false;
    capture.capture_active = false;
    capture.radial_started = false;
    capture.pressed_ms = 0;
}

bool IsSwitchSpell2Present()
{
    return in_game_pad::PollInput(kSwitchSpell2Input);
}

bool IsSwitchItem2Present()
{
    return in_game_pad::PollInput(kSwitchItem2Input);
}

bool IsCaptureActive(CaptureState& capture, bool (*is_input_present)())
{
    if (!gameplay_state::GetCachedNormalGameplayHudState()) return false;
    if (capture.capture_active) return true;
    if (!is_input_present()) return false;

    BeginCapture(capture);
    return true;
}

bool ShouldSuppressSpellSwitch(void* equip_magic_data)
{
    const auto current_equip_magic_data = equip_access::ResolveEquipMagicData();
    return IsCaptureActive(g_spell_capture, &IsSwitchSpell2Present) && current_equip_magic_data != 0 &&
        reinterpret_cast<std::uintptr_t>(equip_magic_data) == current_equip_magic_data;
}

bool ShouldSuppressItemSwitch(void* equip_item_data)
{
    const auto current_equip_item_data = equip_access::ResolveEquipItemData();
    return IsCaptureActive(g_item_capture, &IsSwitchItem2Present) && current_equip_item_data != 0 &&
        reinterpret_cast<std::uintptr_t>(equip_item_data) == current_equip_item_data;
}

void PlayEquipmentChangeSound()
{
    const auto function_address = GetModuleBase() + kEquipmentChangeSoundEventRva;
    const auto vtable_address = GetModuleBase() + kEquipmentChangeSoundEventVtableRva;
    if (!IsReadableMemory(reinterpret_cast<const void*>(function_address), 1) ||
        !IsReadableMemory(reinterpret_cast<const void*>(vtable_address), sizeof(void*))) {
        return;
    }

    EquipmentChangeSoundEvent event = {};
    event.vtable = reinterpret_cast<void*>(vtable_address);
    const auto play_event = reinterpret_cast<EquipmentChangeSoundEventFn>(function_address);
    play_event(&event);
}

void HookedEquipmentHudUpdate(void* hud_context, void* arg2, void* arg3)
{
    if (g_original_equipment_hud_update != nullptr) g_original_equipment_hud_update(hud_context, arg2, arg3);
    if (hud_context == nullptr) return;

    const std::uint8_t feedback_state = 1;
    if (g_spell_capture.tap_feedback_pending &&
        WriteGameMemory(reinterpret_cast<std::uintptr_t>(hud_context) + 0x09, feedback_state)) {
        g_spell_capture.tap_feedback_pending = false;
    }

    if (g_item_capture.tap_feedback_pending &&
        WriteGameMemory(reinterpret_cast<std::uintptr_t>(hud_context) + 0x08, feedback_state)) {
        g_item_capture.tap_feedback_pending = false;
    }
}

bool HookedSwitchSpellRequestCheck(void* input_state)
{
    const bool requested = g_original_switch_spell_request_check != nullptr ?
        g_original_switch_spell_request_check(input_state) : false;
    if (requested || IsCaptureActive(g_spell_capture, &IsSwitchSpell2Present)) {
        if (requested) BeginCapture(g_spell_capture);
        if (!g_logged_switch_spell_request_suppression) {
            g_logged_switch_spell_request_suppression = true;
            Log("Suppressed SwitchSpell request check for radial capture.");
        }
        return false;
    }

    return false;
}

bool HookedSwitchItemRequestCheck(void* input_state)
{
    const bool requested = g_original_switch_item_request_check != nullptr ?
        g_original_switch_item_request_check(input_state) : false;
    if (requested || IsCaptureActive(g_item_capture, &IsSwitchItem2Present)) {
        if (requested) BeginCapture(g_item_capture);
        if (!g_logged_switch_item_request_suppression) {
            g_logged_switch_item_request_suppression = true;
            Log("Suppressed SwitchItem request check for radial capture.");
        }
        return false;
    }

    return false;
}

bool HookedSwitchHoldCheck(void* input_state, std::int32_t action)
{
    if (action != kSwitchSpellAction && action != kSwitchItemAction) {
        return g_original_switch_hold_check != nullptr ? g_original_switch_hold_check(input_state, action) : false;
    }

    const bool requested = g_original_switch_hold_check != nullptr ?
        g_original_switch_hold_check(input_state, action) : false;

    if (action == kSwitchSpellAction && (requested || IsCaptureActive(g_spell_capture, &IsSwitchSpell2Present))) {
        if (requested) BeginCapture(g_spell_capture);
        if (!g_logged_switch_spell_hold_suppression) {
            g_logged_switch_spell_hold_suppression = true;
            Log("Suppressed SwitchSpell hold check for radial capture.");
        }
        return false;
    }

    if (action == kSwitchItemAction && (requested || IsCaptureActive(g_item_capture, &IsSwitchItem2Present))) {
        if (requested) BeginCapture(g_item_capture);
        if (!g_logged_switch_item_hold_suppression) {
            g_logged_switch_item_hold_suppression = true;
            Log("Suppressed SwitchItem hold check for radial capture.");
        }
        return false;
    }

    return false;
}

bool HookedSwitchSpellRepeatCheck(void* input_state)
{
    const bool requested = g_original_switch_spell_repeat_check != nullptr ?
        g_original_switch_spell_repeat_check(input_state) : false;
    if (requested || IsCaptureActive(g_spell_capture, &IsSwitchSpell2Present)) {
        if (requested) BeginCapture(g_spell_capture);
        if (!g_logged_switch_spell_repeat_suppression) {
            g_logged_switch_spell_repeat_suppression = true;
            Log("Suppressed SwitchSpell repeat check for radial capture.");
        }
        return false;
    }

    return false;
}

bool HookedSwitchItemRepeatCheck(void* input_state)
{
    const bool requested = g_original_switch_item_repeat_check != nullptr ?
        g_original_switch_item_repeat_check(input_state) : false;
    if (requested || IsCaptureActive(g_item_capture, &IsSwitchItem2Present)) {
        if (requested) BeginCapture(g_item_capture);
        if (!g_logged_switch_item_repeat_suppression) {
            g_logged_switch_item_repeat_suppression = true;
            Log("Suppressed SwitchItem repeat check for radial capture.");
        }
        return false;
    }

    return false;
}

bool HookedCanSwitchSpell(void* equip_magic_data)
{
    if (ShouldSuppressSpellSwitch(equip_magic_data)) {
        if (!g_logged_can_switch_spell_suppression) {
            g_logged_can_switch_spell_suppression = true;
            Log("Suppressed SwitchSpell can-switch check while radial capture is active.");
        }
        return false;
    }

    return g_original_can_switch_spell != nullptr ? g_original_can_switch_spell(equip_magic_data) : false;
}

void HookedSwitchSpellNext(void* equip_magic_data)
{
    if (ShouldSuppressSpellSwitch(equip_magic_data)) {
        if (!g_logged_switch_spell_next_suppression) {
            g_logged_switch_spell_next_suppression = true;
            Log("Suppressed SwitchSpell next-slot writer while radial capture is active.");
        }
        return;
    }

    if (g_original_switch_spell_next != nullptr) g_original_switch_spell_next(equip_magic_data);
}

void HookedSwitchItemNext(void* equip_item_data)
{
    if (ShouldSuppressItemSwitch(equip_item_data)) {
        if (!g_logged_switch_item_next_suppression) {
            g_logged_switch_item_next_suppression = true;
            Log("Suppressed SwitchItem next-slot writer while radial capture is active.");
        }
        return;
    }

    if (g_original_switch_item_next != nullptr) g_original_switch_item_next(equip_item_data);
}

void PassThroughShortSwitchSpellTap()
{
    if (g_original_switch_spell_next == nullptr) return;

    const auto equip_magic_data = equip_access::ResolveEquipMagicData();
    if (!equip_magic_data) return;

    if (!g_logged_switch_spell_tap_passthrough) {
        g_logged_switch_spell_tap_passthrough = true;
        Log("Passing through short SwitchSpell tap via selected-slot writer.");
    }
    g_spell_capture.tap_feedback_pending = true;
    g_original_switch_spell_next(reinterpret_cast<void*>(equip_magic_data));
    PlayEquipmentChangeSound();
}

void PassThroughShortSwitchItemTap()
{
    if (g_original_switch_item_next == nullptr) return;

    const auto equip_item_data = equip_access::ResolveEquipItemData();
    if (!equip_item_data) return;

    if (!g_logged_switch_item_tap_passthrough) {
        g_logged_switch_item_tap_passthrough = true;
        Log("Passing through short SwitchItem tap via selected-slot writer.");
    }
    g_item_capture.tap_feedback_pending = true;
    g_original_switch_item_next(reinterpret_cast<void*>(equip_item_data));
    PlayEquipmentChangeSound();
}

void UpdateSpellRadialState(float selection_x, float selection_y)
{
    const bool is_down = IsSwitchSpell2Present();
    const auto now = GetTickCount64();

    if (is_down) {
        if (!g_spell_capture.input_down) BeginCapture(g_spell_capture);

        if (!g_spell_capture.radial_started && now - g_spell_capture.pressed_ms >= kRadialHoldThresholdMs) {
            g_spell_capture.radial_started = true;
            radial_input::HandleActionState(true, false, selection_x, selection_y);
        } else if (g_spell_capture.radial_started) {
            radial_input::HandleActionState(true, false, selection_x, selection_y);
        }
        return;
    }

    if (!g_spell_capture.input_down) return;

    const bool radial_started = g_spell_capture.radial_started;
    const auto held_ms = now - g_spell_capture.pressed_ms;
    ResetCapture(g_spell_capture);

    if (radial_started) {
        radial_input::HandleActionState(false, false, selection_x, selection_y);
    } else if (held_ms < kRadialHoldThresholdMs) {
        PassThroughShortSwitchSpellTap();
    }
}

void UpdateItemRadialState(float selection_x, float selection_y)
{
    const bool is_down = IsSwitchItem2Present();
    const auto now = GetTickCount64();

    if (is_down) {
        if (!g_item_capture.input_down) BeginCapture(g_item_capture);

        if (!g_item_capture.radial_started && now - g_item_capture.pressed_ms >= kRadialHoldThresholdMs) {
            g_item_capture.radial_started = true;
            radial_input::HandleActionState(false, true, selection_x, selection_y);
        } else if (g_item_capture.radial_started) {
            radial_input::HandleActionState(false, true, selection_x, selection_y);
        }
        return;
    }

    if (!g_item_capture.input_down) return;

    const bool radial_started = g_item_capture.radial_started;
    const auto held_ms = now - g_item_capture.pressed_ms;
    ResetCapture(g_item_capture);

    if (radial_started) {
        radial_input::HandleActionState(false, false, selection_x, selection_y);
    } else if (held_ms < kRadialHoldThresholdMs) {
        PassThroughShortSwitchItemTap();
    }
}

void UpdateRadialInputStates()
{
    const float selection_x = radial_camera::ConsumeSelectionX();
    const float selection_y = radial_camera::ConsumeSelectionY();

    if (!gameplay_state::GetCachedNormalGameplayHudState()) {
        ResetCapture(g_spell_capture);
        ResetCapture(g_item_capture);
        return;
    }

    UpdateSpellRadialState(selection_x, selection_y);
    UpdateItemRadialState(selection_x, selection_y);
}

template <typename Fn>
void TryInstallHook(const char* name, std::uintptr_t rva, void* detour, Fn*& original, bool& installed, bool& failed)
{
    if (installed || failed) return;

    const auto hook_address = GetModuleBase() + rva;
    if (!IsReadableMemory(reinterpret_cast<const void*>(hook_address), 1)) {
        failed = true;
        Log("%s hook failed: target RVA is not readable.", name);
        return;
    }

    const MH_STATUS create_status = MH_CreateHook(reinterpret_cast<void*>(hook_address), detour,
        reinterpret_cast<void**>(&original));
    if (create_status != MH_OK) {
        failed = true;
        Log("%s hook creation failed: status=%d.", name, static_cast<int>(create_status));
        return;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<void*>(hook_address));
    if (enable_status != MH_OK) {
        failed = true;
        Log("%s hook enable failed: status=%d.", name, static_cast<int>(enable_status));
        return;
    }

    installed = true;
    Log("%s hook installed at rva=0x%llX.", name, static_cast<unsigned long long>(rva));
}

void TryInstallHooks()
{
    TryInstallHook("Equipment HUD update", kEquipmentHudUpdateRva, reinterpret_cast<void*>(&HookedEquipmentHudUpdate),
        g_original_equipment_hud_update, g_equipment_hud_update_hook_installed, g_equipment_hud_update_hook_failed);
    TryInstallHook("SwitchSpell request-check", kSwitchSpellRequestCheckRva,
        reinterpret_cast<void*>(&HookedSwitchSpellRequestCheck), g_original_switch_spell_request_check,
        g_switch_spell_request_hook_installed, g_switch_spell_request_hook_failed);
    TryInstallHook("SwitchItem request-check", kSwitchItemRequestCheckRva,
        reinterpret_cast<void*>(&HookedSwitchItemRequestCheck), g_original_switch_item_request_check,
        g_switch_item_request_hook_installed, g_switch_item_request_hook_failed);
    TryInstallHook("Switch hold-check", kSwitchSpellHoldCheckRva, reinterpret_cast<void*>(&HookedSwitchHoldCheck),
        g_original_switch_hold_check, g_switch_hold_hook_installed, g_switch_hold_hook_failed);
    TryInstallHook("SwitchSpell repeat-check", kSwitchSpellRepeatCheckRva,
        reinterpret_cast<void*>(&HookedSwitchSpellRepeatCheck), g_original_switch_spell_repeat_check,
        g_switch_spell_repeat_hook_installed, g_switch_spell_repeat_hook_failed);
    TryInstallHook("SwitchItem repeat-check", kSwitchItemRepeatCheckRva,
        reinterpret_cast<void*>(&HookedSwitchItemRepeatCheck), g_original_switch_item_repeat_check,
        g_switch_item_repeat_hook_installed, g_switch_item_repeat_hook_failed);
    TryInstallHook("SwitchSpell can-switch", kCanSwitchSpellRva, reinterpret_cast<void*>(&HookedCanSwitchSpell),
        g_original_can_switch_spell, g_can_switch_spell_hook_installed, g_can_switch_spell_hook_failed);
    TryInstallHook("SwitchSpell next-slot", kSwitchSpellNextRva, reinterpret_cast<void*>(&HookedSwitchSpellNext),
        g_original_switch_spell_next, g_switch_spell_next_hook_installed, g_switch_spell_next_hook_failed);
    TryInstallHook("SwitchItem next-slot", kSwitchItemNextRva, reinterpret_cast<void*>(&HookedSwitchItemNext),
        g_original_switch_item_next, g_switch_item_next_hook_installed, g_switch_item_next_hook_failed);
}

}  // namespace

bool Initialize()
{
    TryInstallHooks();
    return true;
}

void SampleFrame()
{
    TryInstallHooks();
    UpdateRadialInputStates();
}

bool IsRadialActive()
{
    return g_spell_capture.radial_started || g_item_capture.radial_started || radial_menu::IsOpen();
}

}  // namespace radial_menu_mod::radial_switch
