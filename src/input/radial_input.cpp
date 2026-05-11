#include "input/radial_input.h"

#include "core/common.h"
#include "game/state/gameplay_state.h"
#include "render/ui/radial_menu.h"

namespace radial_menu_mod::radial_input {
namespace {

enum class RadialKind {
    none,
    spells,
    items,
};

struct ControllerContext {
    bool trigger_down = false;
    bool radial_opened = false;
    RadialKind active_kind = RadialKind::none;
};

ControllerContext g_controller = {};
std::vector<SpellSlot> g_open_spell_slots;
RadialKind g_open_kind = RadialKind::none;

bool CanStartRadialInput(RadialKind kind)
{
    if (!gameplay_state::GetCachedNormalGameplayHudState()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("Action radial input blocked because cached gameplay HUD state is not ready.");
        }
        return false;
    }
    return kind != RadialKind::none;
}

void ResetTriggerState(ControllerContext& controller)
{
    controller.trigger_down = false;
    controller.radial_opened = false;
    controller.active_kind = RadialKind::none;
}

void BeginTriggerHold(ControllerContext& controller, RadialKind kind)
{
    controller.trigger_down = true;
    controller.radial_opened = false;
    controller.active_kind = kind;
    Log("Action radial hold started (kind=%s).", kind == RadialKind::items ? "items" : "spells");
}

void LoadOpenRadialSlots(RadialKind kind)
{
    g_open_spell_slots = kind == RadialKind::items ? GetQuickItems() : GetMemorizedSpells();
}

int CurrentSelectionFor(RadialKind kind)
{
    return kind == RadialKind::items ? GetCurrentQuickItemSlot() : GetCurrentSpellSlot();
}

bool OpenRadial(ControllerContext& controller)
{
    LoadOpenRadialSlots(controller.active_kind);
    if (g_open_spell_slots.empty()) {
        Log("Action radial open failed because no slots were available (kind=%s).",
            controller.active_kind == RadialKind::items ? "items" : "spells");
        g_open_kind = RadialKind::none;
        ResetTriggerState(controller);
        return false;
    }

    controller.radial_opened = true;
    g_open_kind = controller.active_kind;

    int initial_selection = CurrentSelectionFor(controller.active_kind);
    if (initial_selection < 0) initial_selection = 0;

    radial_menu::Open(initial_selection);
    Log("Action radial opened (kind=%s slots=%zu initial=%d).",
        controller.active_kind == RadialKind::items ? "items" : "spells",
        g_open_spell_slots.size(),
        initial_selection);
    return true;
}

void UpdateRadialSelection(const ControllerContext& controller, float right_stick_x, float right_stick_y)
{
    if (!controller.radial_opened) return;
    radial_menu::UpdateSelectionFromStick(right_stick_x, right_stick_y, g_open_spell_slots.size());
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

void CloseRadial()
{
    radial_menu::Close();
    g_open_spell_slots.clear();
    g_open_kind = RadialKind::none;
}

void HandleTriggerRelease(ControllerContext& controller)
{
    if (controller.radial_opened) {
        ConfirmRadialSelection(controller.active_kind);
        CloseRadial();
        Log("Action radial closed by release.");
    }

    ResetTriggerState(controller);
}

}  // namespace

void Reset()
{
    radial_menu::Close();
    g_controller = {};
    g_open_spell_slots.clear();
    g_open_kind = RadialKind::none;
}

void HandleActionState(bool spell_held, bool item_held, float right_stick_x, float right_stick_y)
{
    auto& controller = g_controller;
    const RadialKind pressed_kind = spell_held ? RadialKind::spells : (item_held ? RadialKind::items : RadialKind::none);
    const bool trigger_pressed = controller.active_kind != RadialKind::none
        ? pressed_kind == controller.active_kind
        : pressed_kind != RadialKind::none;
    bool checked_gameplay_state = false;

    if (pressed_kind != RadialKind::none && !controller.trigger_down) {
        if (!CanStartRadialInput(pressed_kind)) return;

        BeginTriggerHold(controller, pressed_kind);
        checked_gameplay_state = true;
    }

    if (controller.trigger_down && !controller.radial_opened && !checked_gameplay_state) {
        if (!gameplay_state::GetCachedNormalGameplayHudState()) {
            ResetTriggerState(controller);
            return;
        }
    }

    if (controller.trigger_down && trigger_pressed && !controller.radial_opened) {
        if (!OpenRadial(controller)) return;
    }

    UpdateRadialSelection(controller, right_stick_x, right_stick_y);

    if (controller.trigger_down && !trigger_pressed) {
        HandleTriggerRelease(controller);
    }
}

void GetActionSuppressionState(bool& suppress_spell_switch, bool& suppress_item_switch)
{
    suppress_spell_switch = false;
    suppress_item_switch = false;

    if (g_controller.trigger_down || g_controller.radial_opened) {
        if (g_controller.active_kind == RadialKind::spells) suppress_spell_switch = true;
        if (g_controller.active_kind == RadialKind::items) suppress_item_switch = true;
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
        ? "Right Stick   Release D-pad Down Confirm"
        : "Right Stick   Release D-pad Up Confirm";
}

}  // namespace radial_menu_mod::radial_input
