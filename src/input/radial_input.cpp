#include "input/radial_input.h"

#include "core/common.h"
#include "game/state/gameplay_state.h"
#include "render/ui/radial_menu.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace radial_spell_menu::radial_input {
namespace {

constexpr ULONGLONG kHoldThresholdMs = 180;
constexpr ULONGLONG kTapPressMs = 60;
constexpr ULONGLONG kTapReleaseMs = 90;
constexpr WORD kSpellButton = XINPUT_GAMEPAD_DPAD_UP;
constexpr WORD kItemButton = XINPUT_GAMEPAD_DPAD_DOWN;
constexpr DWORD kControllerCount = 4;

enum class RadialKind {
    none,
    spells,
    items,
};

enum class SyntheticInputPhase {
    idle,
    tap_press,
    tap_release,
};

struct ControllerContext {
    bool trigger_down = false;
    bool radial_opened = false;
    WORD trigger_button = 0;
    WORD synthetic_button = 0;
    RadialKind active_kind = RadialKind::none;
    ULONGLONG pressed_at = 0;
    SyntheticInputPhase synthetic_phase = SyntheticInputPhase::idle;
    ULONGLONG synthetic_phase_until = 0;
};

std::array<ControllerContext, kControllerCount> g_controllers = {};
std::vector<SpellSlot> g_open_spell_slots;
RadialKind g_open_kind = RadialKind::none;

bool CanStartRadialInput(RadialKind kind)
{
    if (!gameplay_state::IsNormalGameplayHudState()) return false;
    return kind == RadialKind::spells ? !GetMemorizedSpells().empty() : true;
}

RadialKind KindForButton(WORD button)
{
    if (button == kSpellButton) return RadialKind::spells;
    if (button == kItemButton) return RadialKind::items;
    return RadialKind::none;
}

WORD FirstPressedTrigger(WORD buttons)
{
    if ((buttons & kSpellButton) != 0) return kSpellButton;
    if ((buttons & kItemButton) != 0) return kItemButton;
    return 0;
}

float NormalizeThumbAxis(SHORT value, SHORT deadzone)
{
    if (std::abs(static_cast<int>(value)) <= deadzone) return 0.0f;

    const float normalized = static_cast<float>(value) / 32767.0f;
    return (normalized < -1.0f) ? -1.0f : ((normalized > 1.0f) ? 1.0f : normalized);
}

void QueueTap(ControllerContext& controller, ULONGLONG now)
{
    controller.synthetic_phase = SyntheticInputPhase::tap_press;
    controller.synthetic_phase_until = now + kTapPressMs;
}

void ApplyVirtualTap(ControllerContext& controller, XINPUT_STATE* state, ULONGLONG now)
{
    if (controller.synthetic_button == 0) return;

    state->Gamepad.wButtons &= ~controller.synthetic_button;

    switch (controller.synthetic_phase) {
    case SyntheticInputPhase::idle:
        controller.synthetic_button = 0;
        return;

    case SyntheticInputPhase::tap_press:
        state->Gamepad.wButtons |= controller.synthetic_button;
        if (now >= controller.synthetic_phase_until) {
            controller.synthetic_phase = SyntheticInputPhase::tap_release;
            controller.synthetic_phase_until = now + kTapReleaseMs;
        }
        return;

    case SyntheticInputPhase::tap_release:
        if (now < controller.synthetic_phase_until) return;

        controller.synthetic_phase = SyntheticInputPhase::idle;
        controller.synthetic_phase_until = 0;
        controller.synthetic_button = 0;
        return;
    }
}

void ResetTriggerState(ControllerContext& controller)
{
    controller.trigger_down = false;
    controller.radial_opened = false;
    controller.trigger_button = 0;
    controller.active_kind = RadialKind::none;
}

void BeginTriggerHold(ControllerContext& controller, WORD button, RadialKind kind, ULONGLONG now)
{
    controller.trigger_down = true;
    controller.radial_opened = false;
    controller.trigger_button = button;
    controller.active_kind = kind;
    controller.pressed_at = now;
}

void LoadOpenRadialSlots(RadialKind kind)
{
    g_open_spell_slots = kind == RadialKind::items ? GetQuickItems() : GetMemorizedSpells();
}

int CurrentSelectionFor(RadialKind kind)
{
    return kind == RadialKind::items ? GetCurrentQuickItemSlot() : GetCurrentSpellSlot();
}

bool OpenRadial(ControllerContext& controller, DWORD user_index)
{
    LoadOpenRadialSlots(controller.active_kind);
    if (g_open_spell_slots.empty()) {
        g_open_kind = RadialKind::none;
        ResetTriggerState(controller);
        return false;
    }

    controller.radial_opened = true;
    g_open_kind = controller.active_kind;

    int initial_selection = CurrentSelectionFor(controller.active_kind);
    if (initial_selection < 0) initial_selection = 0;

    radial_menu::Open(initial_selection);
    Log("Opening radial %s menu for controller %lu.",
        controller.active_kind == RadialKind::items ? "quick item" : "spell",
        static_cast<unsigned long>(user_index));
    return true;
}

void UpdateRadialSelection(const ControllerContext& controller, XINPUT_STATE* state)
{
    if (!controller.radial_opened) return;

    const float right_x = NormalizeThumbAxis(state->Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    const float right_y = NormalizeThumbAxis(state->Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    radial_menu::UpdateSelectionFromStick(right_x, right_y, g_open_spell_slots.size());

    state->Gamepad.sThumbRX = 0;
    state->Gamepad.sThumbRY = 0;
}

void ConfirmRadialSelection(const ControllerContext& controller)
{
    const int selected_slot = radial_menu::GetSelectedSlot();
    if (selected_slot < 0 || selected_slot >= static_cast<int>(g_open_spell_slots.size())) return;

    const auto selected_index = static_cast<std::size_t>(selected_slot);
    if (controller.active_kind == RadialKind::items) {
        (void)SwitchToQuickItemSlot(g_open_spell_slots[selected_index].slot_index);
    } else {
        (void)SwitchToSpellSlot(g_open_spell_slots[selected_index].slot_index);
    }
}

void CloseRadial(DWORD user_index)
{
    radial_menu::Close();
    g_open_spell_slots.clear();
    g_open_kind = RadialKind::none;
    Log("Closed radial menu for controller %lu.", static_cast<unsigned long>(user_index));
}

void HandleTriggerRelease(ControllerContext& controller, DWORD user_index, XINPUT_STATE* state, ULONGLONG now)
{
    if (controller.radial_opened) {
        ConfirmRadialSelection(controller);
        CloseRadial(user_index);
    } else {
        controller.synthetic_button = controller.trigger_button;
        QueueTap(controller, now);
    }

    const WORD released_button = controller.trigger_button;
    ResetTriggerState(controller);
    state->Gamepad.wButtons &= ~released_button;
}

void SuppressHeldTrigger(const ControllerContext& controller, XINPUT_STATE* state)
{
    if ((controller.trigger_down || controller.radial_opened) && controller.trigger_button != 0) {
        state->Gamepad.wButtons &= ~controller.trigger_button;
    }
}

}  // namespace

void Reset()
{
    radial_menu::Close();
    g_controllers = {};
    g_open_spell_slots.clear();
    g_open_kind = RadialKind::none;
}

void HandleControllerState(DWORD user_index, XINPUT_STATE* state)
{
    if (state == nullptr || user_index >= g_controllers.size()) return;

    auto& controller = g_controllers[user_index];
    const ULONGLONG now = GetTickCount64();
    const WORD pressed_trigger = FirstPressedTrigger(state->Gamepad.wButtons);
    const bool physical_trigger_pressed = controller.trigger_button != 0
        ? ((state->Gamepad.wButtons & controller.trigger_button) != 0)
        : (pressed_trigger != 0);

    if (pressed_trigger != 0 && !controller.trigger_down) {
        const RadialKind kind = KindForButton(pressed_trigger);
        if (!CanStartRadialInput(kind)) return;

        BeginTriggerHold(controller, pressed_trigger, kind, now);
    }

    if (controller.trigger_down && !controller.radial_opened && !gameplay_state::IsNormalGameplayHudState()) {
        ResetTriggerState(controller);
        return;
    }

    if (controller.trigger_down && physical_trigger_pressed && !controller.radial_opened &&
        (now - controller.pressed_at) >= kHoldThresholdMs) {
        if (!OpenRadial(controller, user_index)) return;
    }

    UpdateRadialSelection(controller, state);

    if (controller.trigger_down && !physical_trigger_pressed) {
        HandleTriggerRelease(controller, user_index, state, now);
    }

    SuppressHeldTrigger(controller, state);
    ApplyVirtualTap(controller, state, now);
}

const std::vector<SpellSlot>& GetOpenSpellSlots()
{
    return g_open_spell_slots;
}

const char* GetOpenMenuTitle()
{
    return g_open_kind == RadialKind::items ? "Quick Item" : "Quick Spell";
}

const char* GetOpenMenuControls()
{
    return g_open_kind == RadialKind::items
        ? "Right Stick Rotate   Release D-pad Down Confirm"
        : "Right Stick Rotate   Release D-pad Up Confirm";
}

}  // namespace radial_spell_menu::radial_input
