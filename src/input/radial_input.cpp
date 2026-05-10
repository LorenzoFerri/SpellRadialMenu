#include "input/radial_input.h"

#include "core/common.h"
#include "game/state/gameplay_state.h"
#include "render/ui/radial_menu.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace radial_menu_mod::radial_input {
namespace {

constexpr ULONGLONG kHoldThresholdMs = 180;
constexpr ULONGLONG kTapPressMs = 60;
constexpr ULONGLONG kTapReleaseMs = 90;
constexpr WORD kSpellButton = XINPUT_GAMEPAD_DPAD_UP;
constexpr WORD kItemButton = XINPUT_GAMEPAD_DPAD_DOWN;
constexpr DWORD kControllerCount = 4;
constexpr float kMouseSelectionScale = 1.0f / 90.0f;

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

struct KeyboardMouseContext {
    bool trigger_down = false;
    bool radial_opened = false;
    RadialKind active_kind = RadialKind::none;
    ULONGLONG pressed_at = 0;
    float selection_x = 0.0f;
    float selection_y = 0.0f;
};

std::array<ControllerContext, kControllerCount> g_controllers = {};
KeyboardMouseContext g_keyboard_mouse = {};
std::vector<SpellSlot> g_open_spell_slots;
RadialKind g_open_kind = RadialKind::none;

bool CanStartRadialInput(RadialKind kind)
{
    if (!gameplay_state::GetCachedNormalGameplayHudState()) return false;
    return kind != RadialKind::none;
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

void ResetTriggerState(KeyboardMouseContext& input)
{
    input.trigger_down = false;
    input.radial_opened = false;
    input.active_kind = RadialKind::none;
    input.pressed_at = 0;
    input.selection_x = 0.0f;
    input.selection_y = 0.0f;
}

void BeginTriggerHold(ControllerContext& controller, WORD button, RadialKind kind, ULONGLONG now)
{
    controller.trigger_down = true;
    controller.radial_opened = false;
    controller.trigger_button = button;
    controller.active_kind = kind;
    controller.pressed_at = now;
}

void BeginTriggerHold(KeyboardMouseContext& input, RadialKind kind, ULONGLONG now)
{
    input.trigger_down = true;
    input.radial_opened = false;
    input.active_kind = kind;
    input.pressed_at = now;
    input.selection_x = 0.0f;
    input.selection_y = 0.0f;
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
    (void)user_index;
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
    return true;
}

bool OpenRadial(KeyboardMouseContext& input)
{
    LoadOpenRadialSlots(input.active_kind);
    if (g_open_spell_slots.empty()) {
        g_open_kind = RadialKind::none;
        ResetTriggerState(input);
        return false;
    }

    input.radial_opened = true;
    g_open_kind = input.active_kind;

    int initial_selection = CurrentSelectionFor(input.active_kind);
    if (initial_selection < 0) initial_selection = 0;

    radial_menu::Open(initial_selection);
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

void UpdateRadialSelection(const KeyboardMouseContext& input)
{
    if (!input.radial_opened) return;
    radial_menu::UpdateSelectionFromStick(input.selection_x, input.selection_y, g_open_spell_slots.size());
}

void ConfirmRadialSelection(RadialKind active_kind)
{
    const int selected_slot = radial_menu::GetSelectedSlot();
    if (selected_slot < 0 || selected_slot >= static_cast<int>(g_open_spell_slots.size())) return;

    const auto selected_index = static_cast<std::size_t>(selected_slot);
    if (active_kind == RadialKind::items) {
        (void)SwitchToQuickItemSlot(g_open_spell_slots[selected_index].slot_index);
    } else {
        (void)SwitchToSpellSlot(g_open_spell_slots[selected_index].slot_index);
    }
}

void CloseRadial(DWORD user_index)
{
    (void)user_index;
    radial_menu::Close();
    g_open_spell_slots.clear();
    g_open_kind = RadialKind::none;
}

void CloseRadialKeyboardMouse()
{
    radial_menu::Close();
    g_open_spell_slots.clear();
    g_open_kind = RadialKind::none;
}

void HandleTriggerRelease(ControllerContext& controller, DWORD user_index, XINPUT_STATE* state, ULONGLONG now)
{
    const WORD released_button = controller.trigger_button;
    if (controller.radial_opened) {
        ConfirmRadialSelection(controller.active_kind);
        CloseRadial(user_index);
    } else {
        controller.synthetic_button = controller.trigger_button;
        QueueTap(controller, now);
    }

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
    g_keyboard_mouse = {};
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
    bool checked_gameplay_state = false;

    if (pressed_trigger != 0 && !controller.trigger_down) {
        const RadialKind kind = KindForButton(pressed_trigger);
        if (!CanStartRadialInput(kind)) return;

        BeginTriggerHold(controller, pressed_trigger, kind, now);
        checked_gameplay_state = true;
    }

    if (controller.trigger_down && !controller.radial_opened && !checked_gameplay_state) {
        if (!gameplay_state::GetCachedNormalGameplayHudState()) {
            ResetTriggerState(controller);
            return;
        }
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

void HandleKeyboardMouseState(bool spell_held, bool item_held)
{
    const ULONGLONG now = GetTickCount64();
    const RadialKind pressed_kind = spell_held ? RadialKind::spells : (item_held ? RadialKind::items : RadialKind::none);
    const bool trigger_pressed = g_keyboard_mouse.active_kind != RadialKind::none
        ? pressed_kind == g_keyboard_mouse.active_kind
        : pressed_kind != RadialKind::none;

    if (pressed_kind != RadialKind::none && !g_keyboard_mouse.trigger_down) {
        if (!CanStartRadialInput(pressed_kind)) return;
        BeginTriggerHold(g_keyboard_mouse, pressed_kind, now);
    }

    if (g_keyboard_mouse.trigger_down && !g_keyboard_mouse.radial_opened && !gameplay_state::GetCachedNormalGameplayHudState()) {
        ResetTriggerState(g_keyboard_mouse);
        return;
    }

    if (g_keyboard_mouse.trigger_down && trigger_pressed && !g_keyboard_mouse.radial_opened &&
        (now - g_keyboard_mouse.pressed_at) >= kHoldThresholdMs) {
        if (!OpenRadial(g_keyboard_mouse)) return;
    }

    UpdateRadialSelection(g_keyboard_mouse);

    if (g_keyboard_mouse.trigger_down && !trigger_pressed) {
        if (g_keyboard_mouse.radial_opened) {
            ConfirmRadialSelection(g_keyboard_mouse.active_kind);
            CloseRadialKeyboardMouse();
        }
        ResetTriggerState(g_keyboard_mouse);
    }
}

void AddMouseDelta(float delta_x, float delta_y)
{
    if (!g_keyboard_mouse.radial_opened) return;

    g_keyboard_mouse.selection_x += delta_x * kMouseSelectionScale;
    g_keyboard_mouse.selection_y -= delta_y * kMouseSelectionScale;

    const float length_sq = g_keyboard_mouse.selection_x * g_keyboard_mouse.selection_x +
        g_keyboard_mouse.selection_y * g_keyboard_mouse.selection_y;
    if (length_sq > 1.0f) {
        const float inv_length = 1.0f / std::sqrt(length_sq);
        g_keyboard_mouse.selection_x *= inv_length;
        g_keyboard_mouse.selection_y *= inv_length;
    }
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
        ? "Right Stick / Mouse Move   Release D-pad Down / Caps Lock Confirm"
        : "Right Stick / Mouse Move   Release D-pad Up / Tab Confirm";
}

}  // namespace radial_menu_mod::radial_input
